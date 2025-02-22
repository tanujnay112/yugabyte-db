import React, { Component, Fragment } from 'react';
import { YBPanelItem } from '../../../panels';
import { Row, Col, DropdownButton, MenuItem } from 'react-bootstrap';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { Field } from 'formik';
import * as Yup from 'yup';
import { YBButton } from '../../../common/forms/fields';
import { YBModalForm } from '../../../common/forms';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import moment from 'moment';
import { isNotHidden, isDisabled } from '../../../../utils/LayoutUtils';

import './certificates.scss';
import { AddCertificateFormContainer } from './';
import { CertificateDetails } from './CertificateDetails';
import { api } from '../../../../redesign/helpers/api';
import { YBFormInput } from '../../../common/forms/fields';
import { AssociatedUniverse } from '../../../common/associatedUniverse/AssociatedUniverse';
import { YBConfirmModal } from '../../../modals';

const validationSchema = Yup.object().shape({
  username: Yup.string().required('Enter username for certificate')
});

const initialValues = {
  username: 'postgres'
};

export const MODES = {
  CREATE: 'CREATE',
  EDIT: 'EDIT'
};

const downloadAllFilesInObject = (data) => {
  Object.entries(data).forEach((file) => {
    const [filename, content] = file;
    setTimeout(() => {
      const element = document.createElement('a');
      element.setAttribute('href', 'data:text/plain;charset=utf-8,' + encodeURIComponent(content));
      element.setAttribute('download', filename);

      element.style.display = 'none';
      document.body.appendChild(element);
      element.click();
      document.body.removeChild(element);
    });
  });
};

class DownloadCertificateForm extends Component {
  handleDownload = (values) => {
    const { certificate, handleSubmit, onHide } = this.props;
    const data = {
      ...certificate,
      username: values.username
    };
    handleSubmit(data);
    onHide();
  };

  render() {
    const { visible, onHide, certificate } = this.props;
    return (
      <YBModalForm
        validationSchema={validationSchema}
        initialValues={initialValues}
        onFormSubmit={this.handleDownload}
        formName={'downloadCertificate'}
        title={`Download YSQL Certificate ${certificate.name}`}
        id="download-cert-modal"
        visible={visible}
        onHide={onHide}
        submitLabel={'Download'}
      >
        <div className="info-text">
          Clicking download will generate <code>.crt</code> & <code>.key</code> files
        </div>
        <Row>
          <Col lg={5}>
            <Field
              name="username"
              type="text"
              label="Username"
              component={YBFormInput}
              infoContent="Connect to the database using this username for certificate-based authentication"
            />
          </Col>
        </Row>
      </YBModalForm>
    );
  }
}

class Certificates extends Component {
  state = {
    showSubmitting: false,
    selectedCert: {},
    associatedUniverses: [],
    isVisibleModal: false,
    mode: MODES.CREATE
  };
  getDateColumn = (key) => (item, row) => {
    if (key in row) {
      return moment.utc(row[key], 'YYYY-MM-DD hh:mm:ss a').local().calendar();
    } else {
      return null;
    }
  };

  downloadYCQLCertificates = (values) => {
    const { fetchClientCert } = this.props;
    this.setState({ showSubmitting: true });
    fetchClientCert(values.uuid, values)
      .then((data) => {
        downloadAllFilesInObject(data);
      })
      .finally(() => this.setState({ showSubmitting: false }));
  };

  downloadRootCertificate = (values) => {
    this.setState({ showSubmitting: true });
    this.props
      .fetchRootCert(values.uuid)
      .then((data) => {
        downloadAllFilesInObject(data);
      })
      .finally(() => this.setState({ showSubmitting: false }));
  };

  showDeleteCertificateModal = (certificateModal) => {
    this.setState({ certificateModal });
    this.props.showConfirmDeleteModal();
  };

  showCertProperties = (item, row) => {
    return (
      <div>
        <a
          className="show_details"
          onClick={(e) => {
            this.setState({ selectedCert: row });
            this.props.showCertificateDetailsModal();
            e.preventDefault();
          }}
          href="/"
        >
          Show details
        </a>
      </div>
    );
  };

  /**
   * Delete the root certificate if certificate is safe to remove,
   * i.e - Certificate is not attached to any universe for current user.
   *
   * @param certificateUUID Unique id of certificate.
   */
  deleteCertificate = (certificateUUID) => {
    const {
      customer: { currentCustomer }
    } = this.props;

    api
      .deleteCertificate(certificateUUID, currentCustomer?.data?.uuid)
      .then(() => this.props.fetchCustomerCertificates())
      .catch((err) => console.error(`Failed to delete certificate ${certificateUUID}`, err));
  };

  formatActionButtons = (cell, row) => {
    const downloadEnabled = ['SelfSigned', 'HashicorpVault'].includes(row.type);
    const deleteDisabled = row.inUse;
    const payload = {
      name: row.name,
      uuid: row.uuid,
      creationTime: row.creationTime,
      expiryDate: row.expiryDate,
      universeDetails: row.universeDetails
    };
    const disableCertEdit = row.type !== 'HashicorpVault';
    // TODO: Replace dropdown option + modal with a side panel
    return (
      <DropdownButton className="btn btn-default" title="Actions" id="bg-nested-dropdown" pullRight>
        <MenuItem
          onClick={() => {
            if (row.customCertInfo) {
              Object.assign(payload, row.customCertInfo);
            }
            this.setState({ selectedCert: row });
            this.props.showCertificateDetailsModal();
          }}
        >
          <i className="fa fa-info-circle"></i> Details
        </MenuItem>
        <MenuItem
          disabled={disableCertEdit}
          onClick={() => {
            if (!disableCertEdit) {
              this.setState({ mode: MODES.EDIT, selectedCert: row }, () => {
                this.props.showAddCertificateModal();
              });
            }
          }}
        >
          <i className="fa fa-edit"></i> Edit Certificate
        </MenuItem>
        <MenuItem
          onClick={() => {
            if (downloadEnabled) {
              this.setState({ selectedCert: payload });
              this.props.showDownloadCertificateModal();
            }
          }}
          disabled={!downloadEnabled}
        >
          <i className="fa fa-download"></i> Download YSQL Cert
        </MenuItem>
        <MenuItem
          onClick={() => {
            downloadEnabled && this.downloadRootCertificate(row);
          }}
          disabled={!downloadEnabled}
        >
          <i className="fa fa-download"></i> Download Root CA Cert
        </MenuItem>
        <MenuItem
          onClick={() => {
            !deleteDisabled && this.showDeleteCertificateModal(payload);
          }}
          disabled={deleteDisabled}
          title={deleteDisabled ? 'In use certificates cannot be deleted' : null}
        >
          <i className="fa fa-trash"></i> Delete Certificate
        </MenuItem>
        <MenuItem
          onClick={() => {
            this.setState({
              associatedUniverses: [...payload?.universeDetails],
              isVisibleModal: true
            });
          }}
        >
          <i className="fa fa-eye"></i> Show Universes
        </MenuItem>
      </DropdownButton>
    );
  };

  /**
   * Close the modal by setting the local flag.
   */
  closeModal = () => {
    this.setState({ isVisibleModal: false, mode: MODES.CREATE });
  };

  render() {
    const {
      customer: { currentCustomer, userCertificates },
      modal: { showModal, visibleModal },
      showAddCertificateModal,
      featureFlags
    } = this.props;

    const { showSubmitting, associatedUniverses, isVisibleModal } = this.state;

    //feature flagging
    const isHCVaultEnabled =
      featureFlags.test.enableHCVaultEAT || featureFlags.released.enableHCVaultEAT;

    const certificateArray = getPromiseState(userCertificates).isSuccess()
      ? userCertificates.data
        .reduce((allCerts, cert) => {
          const certInfo = {
            type: cert.certType,
            uuid: cert.uuid,
            name: cert.label,
            expiryDate: cert.expiryDate,
            certificate: cert.certificate,
            creationTime: cert.startDate,
            privateKey: cert.privateKey,
            customCertInfo: cert.customCertInfo,
            inUse: cert.inUse,
            universeDetails: cert.universeDetails,
            hcVaultCertParams: cert.customHCPKICertInfo
          };

          const isVaultCert = cert.certType === 'HashicorpVault';
          if (isVaultCert) {
            isHCVaultEnabled && allCerts.push(certInfo);
          } else allCerts.push(certInfo);

          return allCerts;
        }, [])
        .sort((a, b) => new Date(b.creationTime) - new Date(a.creationTime))
      : [];

    return (
      <div id="page-wrapper">
        <YBPanelItem
          header={
            <Row className="header-row">
              <Col xs={6}>
                <h2>Certificates</h2>
              </Col>
              <Col xs={6} className="universe-table-header-action">
                {isNotHidden(currentCustomer.data.features, 'universe.create') && (
                  <YBButton
                    btnClass="universe-button btn btn-lg btn-orange"
                    onClick={() => {
                      this.setState({ mode: MODES.CREATE }, () => {
                        showAddCertificateModal();
                      });
                    }}
                    disabled={isDisabled(currentCustomer.data.features, 'universe.create')}
                    btnText="Add Certificate"
                    btnIcon="fa fa-plus"
                  />
                )}
              </Col>
            </Row>
          }
          noBackground
          body={
            <Fragment>
              <BootstrapTable
                data={certificateArray}
                search
                multiColumnSearch
                pagination
                className="bs-table-certs"
                trClassName="tr-cert-name"
              >
                <TableHeaderColumn
                  dataField="name"
                  isKey={true}
                  columnClassName="no-border name-column"
                  className="no-border"
                >
                  Name
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="creationTime"
                  dataFormat={this.getDateColumn('creationTime')}
                  columnClassName="no-border name-column"
                  className="no-border"
                >
                  Creation Time
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="expiryDate"
                  dataFormat={this.getDateColumn('expiryDate')}
                  columnClassName="no-border name-column"
                  className="no-border"
                >
                  Expiration
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="base_url"
                  dataFormat={this.showCertProperties}
                  columnClassName="no-border name-column"
                  className="no-border"
                >
                  Properties
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="actions"
                  width="120px"
                  columnClassName="yb-actions-cell"
                  dataFormat={this.formatActionButtons}
                />
              </BootstrapTable>
              <AddCertificateFormContainer
                visible={showModal && visibleModal === 'addCertificateModal'}
                onHide={this.props.closeModal}
                fetchCustomerCertificates={this.props.fetchCustomerCertificates}
                isHCVaultEnabled={isHCVaultEnabled}
                certificate={this.state.selectedCert}
                mode={this.state.mode}
              />
              <CertificateDetails
                visible={showModal && visibleModal === 'certificateDetailsModal'}
                onHide={this.props.closeModal}
                certificate={this.state.selectedCert}
              />
              <DownloadCertificateForm
                handleSubmit={this.downloadYCQLCertificates}
                visible={showModal && visibleModal === 'downloadCertificateModal'}
                onHide={this.props.closeModal}
                certificate={this.state.selectedCert}
              />
              <AssociatedUniverse
                visible={isVisibleModal}
                onHide={this.closeModal}
                associatedUniverses={associatedUniverses}
                title="Certificate"
              />
            </Fragment>
          }
        />
        {showSubmitting && (
          <div className="loading-text">
            <span>Generating certificates...</span>
            <i onClick={() => this.setState({ showSubmitting: false })} className="fa fa-times"></i>
          </div>
        )}
        <YBConfirmModal
          name="deleteCertificateModal"
          title="Delete Certificate"
          hideConfirmModal={this.props.closeModal}
          currentModal="deleteCertificateModal"
          visibleModal={visibleModal}
          onConfirm={() => this.deleteCertificate(this.state.certificateModal?.uuid)}
          confirmLabel="Delete"
          cancelLabel="Cancel"
        >
          Are you sure you want to delete certificate{' '}
          <strong>{this.state.certificateModal?.name}</strong> ?
        </YBConfirmModal>
      </div>
    );
  }
}

export default Certificates;
